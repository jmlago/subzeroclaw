FROM python:3.12-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY . .

RUN groupadd -r agent && useradd -r -g agent -d /app -s /sbin/nologin agent
RUN mkdir -p /agent-data/vault && chown -R agent:agent /agent-data

USER agent

EXPOSE 9090

CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "9090"]
